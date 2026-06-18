// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

/**
 * Cross-driver statement-chunker conformance test.
 *
 * <p>Loads the shared oracle fixture
 * {@code tests/conformance/drivers/chunker_conformance/cases.json} and asserts that
 * {@link SBSQLParser#splitTopLevelStatements(String)} reproduces every {@code expected}
 * statement list exactly. Mirrors
 * {@code tests/conformance/drivers/chunker_conformance/verify_python_reference.py}.
 */
class SBSQLParserChunkerConformanceTest {

    @Test
    void splitterMatchesCrossDriverConformanceFixture() throws IOException {
        Path casesPath = locateCasesJson();
        assertNotNull(casesPath, "could not locate cases.json conformance fixture");

        List<Case> cases = parseCases(Files.readString(casesPath));
        assertFalse(cases.isEmpty(), "fixture contained no cases");

        for (Case c : cases) {
            List<String> actual = SBSQLParser.splitTopLevelStatements(c.input);
            assertEquals(c.expected, actual, "chunker mismatch for case: " + c.name);
        }
    }

    private static Path locateCasesJson() {
        String relative = "tests/conformance/drivers/chunker_conformance/cases.json";
        Path dir = Path.of("").toAbsolutePath();
        for (int depth = 0; depth < 8 && dir != null; depth++) {
            Path candidate = dir.resolve(relative);
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            // Also try the in-repo location where `project/` is the working tree root.
            Path projectCandidate = dir.resolve("project").resolve(relative);
            if (Files.isRegularFile(projectCandidate)) {
                return projectCandidate;
            }
            dir = dir.getParent();
        }
        return null;
    }

    // ---- fixture model + tiny tolerant JSON reader for this specific shape ----

    private static final class Case {
        String name;
        String input;
        List<String> expected;
    }

    private static List<Case> parseCases(String json) {
        Json reader = new Json(json);
        @SuppressWarnings("unchecked")
        Map<String, Object> root = (Map<String, Object>) reader.parseValue();
        @SuppressWarnings("unchecked")
        List<Object> rawCases = (List<Object>) root.get("cases");
        List<Case> out = new ArrayList<>();
        for (Object raw : rawCases) {
            @SuppressWarnings("unchecked")
            Map<String, Object> m = (Map<String, Object>) raw;
            Case c = new Case();
            c.name = (String) m.get("name");
            c.input = (String) m.get("input");
            List<String> expected = new ArrayList<>();
            @SuppressWarnings("unchecked")
            List<Object> rawExpected = (List<Object>) m.get("expected");
            for (Object item : rawExpected) {
                expected.add((String) item);
            }
            c.expected = expected;
            out.add(c);
        }
        return out;
    }

    private static final class Json {
        private final String src;
        private int pos;

        Json(String src) {
            this.src = src;
        }

        Object parseValue() {
            skipWs();
            char ch = src.charAt(pos);
            switch (ch) {
                case '{':
                    return parseObject();
                case '[':
                    return parseArray();
                case '"':
                    return parseString();
                case 't':
                case 'f':
                    return parseBool();
                case 'n':
                    pos += 4;
                    return null;
                default:
                    return parseNumber();
            }
        }

        private Map<String, Object> parseObject() {
            Map<String, Object> m = new LinkedHashMap<>();
            pos++; // {
            skipWs();
            if (src.charAt(pos) == '}') {
                pos++;
                return m;
            }
            while (true) {
                skipWs();
                String key = parseString();
                skipWs();
                pos++; // :
                m.put(key, parseValue());
                skipWs();
                if (src.charAt(pos++) == '}') {
                    break;
                }
                // otherwise it was ',', continue
            }
            return m;
        }

        private List<Object> parseArray() {
            List<Object> list = new ArrayList<>();
            pos++; // [
            skipWs();
            if (src.charAt(pos) == ']') {
                pos++;
                return list;
            }
            while (true) {
                list.add(parseValue());
                skipWs();
                if (src.charAt(pos++) == ']') {
                    break;
                }
                // otherwise it was ',', continue
            }
            return list;
        }

        private String parseString() {
            StringBuilder sb = new StringBuilder();
            pos++; // opening quote
            while (true) {
                char ch = src.charAt(pos++);
                if (ch == '"') {
                    break;
                }
                if (ch == '\\') {
                    char esc = src.charAt(pos++);
                    switch (esc) {
                        case 'n': sb.append('\n'); break;
                        case 't': sb.append('\t'); break;
                        case 'r': sb.append('\r'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case '"': sb.append('"'); break;
                        case '\\': sb.append('\\'); break;
                        case '/': sb.append('/'); break;
                        case 'u':
                            sb.append((char) Integer.parseInt(src.substring(pos, pos + 4), 16));
                            pos += 4;
                            break;
                        default: sb.append(esc);
                    }
                } else {
                    sb.append(ch);
                }
            }
            return sb.toString();
        }

        private Boolean parseBool() {
            if (src.startsWith("true", pos)) {
                pos += 4;
                return Boolean.TRUE;
            }
            pos += 5;
            return Boolean.FALSE;
        }

        private Double parseNumber() {
            int start = pos;
            while (pos < src.length() && "-+.eE0123456789".indexOf(src.charAt(pos)) >= 0) {
                pos++;
            }
            return Double.parseDouble(src.substring(start, pos));
        }

        private void skipWs() {
            while (pos < src.length() && Character.isWhitespace(src.charAt(pos))) {
                pos++;
            }
        }
    }
}
