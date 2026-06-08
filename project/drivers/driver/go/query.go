// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"database/sql/driver"
	"errors"
	"fmt"
	"strings"
)

type normalizedQuery struct {
	sql  string
	args []driver.NamedValue
}

func normalizeQuery(query string, args []driver.NamedValue) (normalizedQuery, error) {
	if len(args) == 0 {
		return normalizedQuery{sql: query, args: nil}, nil
	}
	if hasNamedParams(query) {
		sql, ordered, err := rewriteNamedParams(query, args)
		if err != nil {
			return normalizedQuery{}, err
		}
		return normalizedQuery{sql: sql, args: ordered}, nil
	}
	if strings.Contains(query, "?") {
		sql, ordered, err := rewritePositionalParams(query, args)
		if err != nil {
			return normalizedQuery{}, err
		}
		return normalizedQuery{sql: sql, args: ordered}, nil
	}
	return normalizedQuery{sql: query, args: args}, nil
}

func normalizeCallableQuery(query string, args []driver.NamedValue) (normalizedQuery, error) {
	callableSQL, err := normalizeCallableSQL(query)
	if err != nil {
		return normalizedQuery{}, err
	}
	return normalizeQuery(callableSQL, args)
}

func normalizeCallableSQL(query string) (string, error) {
	trimmed := strings.TrimSpace(query)
	if !strings.HasPrefix(trimmed, "{") || !strings.HasSuffix(trimmed, "}") {
		return query, nil
	}
	if len(trimmed) < 2 {
		return query, nil
	}

	inner := strings.TrimSpace(trimmed[1 : len(trimmed)-1])
	if inner == "" {
		return query, nil
	}

	remaining, ok := trimPrefixFold(inner, "?")
	if ok {
		remaining = strings.TrimSpace(remaining)
		if strings.HasPrefix(remaining, "=") {
			remaining = strings.TrimSpace(remaining[1:])
			remaining, ok = trimPrefixFold(remaining, "call")
			if !ok {
				return query, nil
			}
			invocation, parseErr := parseCallableInvocation(strings.TrimSpace(remaining))
			if parseErr != nil {
				return "", parseErr
			}
			callArgs := ""
			if invocation.hasParens {
				callArgs = invocation.args
			}
			return fmt.Sprintf("select %s(%s) as return_value", invocation.routine, callArgs), nil
		}
	}

	remaining, ok = trimPrefixFold(inner, "call")
	if ok {
		invocation, parseErr := parseCallableInvocation(strings.TrimSpace(remaining))
		if parseErr != nil {
			return "", parseErr
		}
		if invocation.hasParens {
			return fmt.Sprintf("call %s(%s)", invocation.routine, invocation.args), nil
		}
		return fmt.Sprintf("call %s", invocation.routine), nil
	}

	return query, nil
}

func hasNamedParams(query string) bool {
	inString := false
	for i := 0; i+1 < len(query); i++ {
		ch := query[i]
		if ch == '\'' {
			inString = !inString
			continue
		}
		if inString {
			continue
		}
		if ch == ':' && query[i+1] == ':' {
			i++
			continue
		}
		if (ch == '@' || ch == ':') && isIdentStart(query[i+1]) {
			return true
		}
	}
	return false
}

func rewriteNamedParams(query string, args []driver.NamedValue) (string, []driver.NamedValue, error) {
	lookup := map[string]driver.NamedValue{}
	for _, arg := range args {
		if arg.Name != "" {
			lookup[strings.TrimLeft(arg.Name, "@:")] = arg
		}
	}
	var sb strings.Builder
	ordered := make([]driver.NamedValue, 0, len(args))
	inString := false
	for i := 0; i < len(query); {
		ch := query[i]
		if ch == '\'' {
			inString = !inString
			sb.WriteByte(ch)
			i++
			continue
		}
		if !inString && ch == ':' && i+1 < len(query) && query[i+1] == ':' {
			sb.WriteString("::")
			i += 2
			continue
		}
		if !inString && (ch == '@' || ch == ':') && i+1 < len(query) && isIdentStart(query[i+1]) {
			j := i + 1
			for j < len(query) && isIdentPart(query[j]) {
				j++
			}
			name := query[i+1 : j]
			param, ok := lookup[name]
			if !ok {
				return "", nil, errors.New("missing named parameter: " + name)
			}
			ordered = append(ordered, param)
			sb.WriteString("$")
			sb.WriteString(intToString(len(ordered)))
			i = j
			continue
		}
		sb.WriteByte(ch)
		i++
	}
	return sb.String(), ordered, nil
}

func rewritePositionalParams(query string, args []driver.NamedValue) (string, []driver.NamedValue, error) {
	var sb strings.Builder
	ordered := make([]driver.NamedValue, 0, len(args))
	inString := false
	index := 0
	for i := 0; i < len(query); {
		ch := query[i]
		if ch == '\'' {
			inString = !inString
			sb.WriteByte(ch)
			i++
			continue
		}
		if !inString && ch == '?' {
			if index >= len(args) {
				return "", nil, errors.New("not enough parameters")
			}
			ordered = append(ordered, args[index])
			index++
			sb.WriteString("$")
			sb.WriteString(intToString(len(ordered)))
			i++
			continue
		}
		sb.WriteByte(ch)
		i++
	}
	if index < len(args) {
		return "", nil, errors.New("too many parameters")
	}
	return sb.String(), ordered, nil
}

func intToString(value int) string {
	if value == 0 {
		return "0"
	}
	var buf [20]byte
	pos := len(buf)
	for value > 0 {
		pos--
		buf[pos] = byte('0' + value%10)
		value /= 10
	}
	return string(buf[pos:])
}

func isIdentStart(ch byte) bool {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'
}

func isIdentPart(ch byte) bool {
	return isIdentStart(ch) || (ch >= '0' && ch <= '9')
}

func trimPrefixFold(value, prefix string) (string, bool) {
	if len(value) < len(prefix) {
		return value, false
	}
	if strings.EqualFold(value[:len(prefix)], prefix) {
		return value[len(prefix):], true
	}
	return value, false
}

type callableInvocation struct {
	routine   string
	args      string
	hasParens bool
}

func parseCallableInvocation(value string) (callableInvocation, error) {
	open := strings.IndexByte(value, '(')
	if open < 0 {
		routine := strings.TrimSpace(value)
		if routine == "" {
			return callableInvocation{}, errors.New("invalid JDBC escape call syntax")
		}
		return callableInvocation{routine: routine, args: "", hasParens: false}, nil
	}

	inSingle := false
	inDouble := false
	depth := 0
	closeIndex := -1
	for i := open; i < len(value); i++ {
		ch := value[i]
		if ch == '\'' && !inDouble {
			inSingle = !inSingle
			continue
		}
		if ch == '"' && !inSingle {
			inDouble = !inDouble
			continue
		}
		if inSingle || inDouble {
			continue
		}
		if ch == '(' {
			depth++
			continue
		}
		if ch == ')' {
			depth--
			if depth == 0 {
				closeIndex = i
				break
			}
		}
	}
	if closeIndex < 0 {
		return callableInvocation{}, errors.New("invalid JDBC escape call syntax")
	}

	routine := strings.TrimSpace(value[:open])
	if routine == "" {
		return callableInvocation{}, errors.New("invalid JDBC escape call syntax")
	}
	trailing := strings.TrimSpace(value[closeIndex+1:])
	if trailing != "" {
		return callableInvocation{}, errors.New("invalid JDBC escape call syntax")
	}
	args := strings.TrimSpace(value[open+1 : closeIndex])
	return callableInvocation{routine: routine, args: args, hasParens: true}, nil
}
