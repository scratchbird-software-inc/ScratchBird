// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Crypto
import Foundation
import ScratchBird

let pageSizes = Set(["4k", "8k", "16k", "32k", "64k", "128k"])
let routes = Set(["embedded", "ipc_local", "listener-parser", "manager-listener-parser"])
let parserModes = Set(["server-parser", "standalone-parser", "driver-sblr-uuid"])

@main
struct SBIsqlSwift {
    static func main() async {
        do {
            let args = try parseArgs(Array(CommandLine.arguments.dropFirst()))
            let rc = try await run(args)
            Foundation.exit(Int32(rc))
        } catch {
            FileHandle.standardError.write(Data("\(error)\n".utf8))
            Foundation.exit(1)
        }
    }

    static func run(_ args: [String: String]) async throws -> Int {
        try validate(args)
        let summaryPath = try required(args, "--summary")
        let runRoot = URL(fileURLWithPath: summaryPath).deletingLastPathComponent().path
        try FileManager.default.createDirectory(atPath: runRoot, withIntermediateDirectories: true)
        let paths = [
            "events": "\(runRoot)/command-events.jsonl",
            "wire": "\(runRoot)/wire-transcript.jsonl",
            "timing": "\(runRoot)/timing-groups.json",
            "digests": "\(runRoot)/result-digests.json",
            "metadata": "\(runRoot)/metadata-snapshots.json",
            "refusals": "\(runRoot)/security-refusals.json",
            "api": "\(runRoot)/native-api-coverage.json",
            "review": "\(runRoot)/code-example-review.json",
            "junit": "\(runRoot)/junit.xml",
            "stdout": "\(runRoot)/stdout.log",
            "stderr": "\(runRoot)/stderr.log"
        ]
        for path in [
            try required(args, "--output"),
            try required(args, "--error"),
            try required(args, "--diagnostics"),
            try required(args, "--metrics"),
            try required(args, "--transcript"),
            summaryPath
        ] + Array(paths.values) {
            try writeText(path, "")
        }

        var timings: [String: Int64] = [:]
        var api = [
            "ScratchBirdConnection": 0,
            "connect": 0,
            "query": 0,
            "metadataTables": 0,
            "begin": 0,
            "commit": 0,
            "rollback": 0,
            "close": 0
        ]
        var testcases: [[String: Any?]] = []
        var failures: [[String: Any?]] = []
        var digests: [[String: Any?]] = []
        let started = nowNs()
        var connection: ScratchBirdConnection?

        do {
            let config = ScratchBirdConfig(
                host: try required(args, "--host"),
                port: Int(try required(args, "--port")) ?? 3092,
                frontDoorMode: try required(args, "--route") == "manager-listener-parser" ? "manager_proxy" : "direct",
                database: try required(args, "--database"),
                user: try required(args, "--user"),
                password: try required(args, "--password"),
                sslmode: args["--sslmode"] ?? "require",
                sslrootcert: args["--sslrootcert"],
                sslcert: args["--sslcert"],
                sslkey: args["--sslkey"],
                applicationName: "SBIsqlSwift",
                role: args["--role"],
                fetchSize: Int(args["--fetch-size"] ?? "1000") ?? 1000
            )
            let connectStarted = nowNs()
            connection = try await ScratchBirdConnection.connect(config)
            api["ScratchBirdConnection", default: 0] += 1
            api["connect", default: 0] += 1
            addTiming(&timings, "connection", connectStarted)
            try appendJsonl(try required(args, "--transcript"), [
                "event": "connect",
                "driver": "swift",
                "route": try required(args, "--route"),
                "parser_mode": try required(args, "--parser-mode"),
                "page_size": try required(args, "--page-size")
            ])
            try appendJsonl(paths["wire"]!, ["event": "server_admission_required", "driver_or_parser_finality": "forbidden"])

            if args["--create-database"] != nil {
                throw RuntimeError("--create-database is not implemented in the Swift native tool yet")
            }
            if try required(args, "--parser-mode") != "server-parser" {
                throw RuntimeError("\(try required(args, "--parser-mode")) is not yet implemented by the Swift native tool; it fails closed")
            }

            let statements = splitStatements(try readInput(try required(args, "--input")))
            for (index, sql) in statements.enumerated() {
                let statementId = "\(URL(fileURLWithPath: try required(args, "--input")).lastPathComponent):\(index + 1)"
                let group = classify(sql)
                let statementStarted = nowNs()
                var outcome = "success"
                var rowCount = -1
                var resultDigest: String?
                var sqlstate: String?
                var diagnostic: String?
                do {
                    if group == "transaction" {
                        try await runTransaction(connection!, sql, &api)
                        rowCount = 0
                        resultDigest = sha256Text("transaction")
                    } else {
                        let result = try await connection!.query(sql)
                        api["query", default: 0] += 1
                        rowCount = result.rows.count
                        resultDigest = sha256Text(String(describing: result.rows))
                        try appendText(try required(args, "--output"), "\(jsonLine(["statement_id": statementId, "rows": String(describing: result.rows)]))\n")
                    }
                    digests.append(["statement_id": statementId, "row_count": rowCount, "result_digest": resultDigest])
                } catch {
                    outcome = "refusal"
                    sqlstate = "HY000"
                    diagnostic = "\(error)"
                    try appendJsonl(try required(args, "--diagnostics"), ["statement_id": statementId, "sqlstate": sqlstate, "message": diagnostic])
                    try appendText(try required(args, "--error"), "\(statementId): \(diagnostic!)\n")
                    failures.append(["statement_id": statementId, "message": diagnostic])
                    if args["--stop-on-error"] != nil {
                        addTiming(&timings, group, statementStarted)
                        break
                    }
                }
                addTiming(&timings, group, statementStarted)
                let event: [String: Any?] = [
                    "run_id": args["--run-id"] ?? "manual",
                    "driver_name": "swift",
                    "driver_version": "unknown",
                    "route": try required(args, "--route"),
                    "parser_mode": try required(args, "--parser-mode"),
                    "page_size": try required(args, "--page-size"),
                    "namespace": try required(args, "--namespace"),
                    "statement_index": index + 1,
                    "statement_id": statementId,
                    "command_group": group,
                    "sql_hash": sha256Text(sql),
                    "expected_outcome": "success",
                    "actual_outcome": outcome,
                    "sqlstate": sqlstate,
                    "diagnostic_code": diagnostic,
                    "row_count": rowCount,
                    "result_digest": resultDigest,
                    "elapsed_ns": nowNs() - statementStarted,
                    "server_revalidation_state": "required",
                    "mga_authority": "engine",
                    "native_api_surface": "swift"
                ]
                try appendJsonl(paths["events"]!, event)
                testcases.append(event)
            }

            let metadataStarted = nowNs()
            let metadata = try await connection!.metadataTables()
            api["metadataTables", default: 0] += 1
            try writeText(paths["metadata"]!, "\(jsonLine(["tables_digest": sha256Text(String(describing: metadata.rows)), "row_count": metadata.rows.count]))\n")
            addTiming(&timings, "metadata", metadataStarted)
        } catch {
            failures.append(["statement_id": "run", "message": "\(error)"])
            try appendText(paths["stderr"]!, "\(error)\n")
        }

        if let connection {
            try await connection.close()
            api["close", default: 0] += 1
        }

        timings["overall"] = nowNs() - started
        let sslmode = args["--sslmode"] ?? "require"
        let summary: [String: Any?] = [
            "run_id": args["--run-id"] ?? "manual",
            "driver_name": "swift",
            "route": try required(args, "--route"),
            "parser_mode": try required(args, "--parser-mode"),
            "page_size": try required(args, "--page-size"),
            "namespace": try required(args, "--namespace"),
            "sslmode": sslmode,
            "transport_mode": sslmode == "disable" ? "tls_disabled" : "tls_required",
            "status": failures.isEmpty ? "pass" : "fail",
            "failure_count": failures.count,
            "elapsed_ns": timings["overall"] ?? 0,
            "server_revalidation_required": true,
            "driver_or_parser_finality": "forbidden",
            "mga_authority": "engine"
        ]
        try writeText(summaryPath, "\(jsonLine(summary))\n")
        try writeText(try required(args, "--metrics"), "\(jsonLine(timings))\n")
        try writeText(paths["timing"]!, "\(jsonLine(timings))\n")
        try writeText(paths["digests"]!, "\(jsonArray(digests))\n")
        try writeText(paths["refusals"]!, "[]\n")
        try writeText(paths["api"]!, "\(jsonLine(api))\n")
        try writeText(paths["review"]!, "\(jsonLine(["driver": "swift", "public_api_only": true, "shells_out_to_other_driver": false, "source_is_canonical_example": true]))\n")
        try writeText(paths["junit"]!, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"SBIsqlSwift\" tests=\"\(max(testcases.count, 1))\" failures=\"\(failures.count)\">\n  <testcase classname=\"scratchbird.swift\" name=\"run\"></testcase>\n</testsuite>\n")
        try appendText(paths["stdout"]!, "SBIsqlSwift status=\(failures.isEmpty ? "pass" : "fail")\n")
        return failures.isEmpty ? 0 : 1
    }
}

struct RuntimeError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

func parseArgs(_ raw: [String]) throws -> [String: String] {
    var args: [String: String] = [:]
    var index = 0
    while index < raw.count {
        let key = raw[index]
        guard key.hasPrefix("--") else { throw RuntimeError("unexpected positional argument: \(key)") }
        if key == "--stop-on-error" || key == "--create-database" {
            args[key] = "true"
            index += 1
            continue
        }
        guard index + 1 < raw.count, !raw[index + 1].hasPrefix("--") else { throw RuntimeError("missing value for \(key)") }
        args[key] = raw[index + 1]
        index += 2
    }
    return args
}

func validate(_ args: [String: String]) throws {
    guard pageSizes.contains(try required(args, "--page-size")) else { throw RuntimeError("unsupported page size") }
    guard routes.contains(try required(args, "--route")) else { throw RuntimeError("unsupported route") }
    guard parserModes.contains(try required(args, "--parser-mode")) else { throw RuntimeError("unsupported parser mode") }
}

func runTransaction(_ connection: ScratchBirdConnection, _ sql: String, _ api: inout [String: Int]) async throws {
    let first = sql.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: " ").first?.lowercased() ?? ""
    if first == "commit" {
        try await connection.commit()
        api["commit", default: 0] += 1
    } else if first == "rollback" {
        try await connection.rollback()
        api["rollback", default: 0] += 1
    } else {
        try await connection.begin()
        api["begin", default: 0] += 1
    }
}

func required(_ args: [String: String], _ key: String) throws -> String {
    guard let value = args[key], !value.isEmpty else { throw RuntimeError("missing required argument \(key)") }
    return value
}

/// Return the new terminator if `chunk` is a `SET TERM <terminator>` client
/// directive, else `nil`. Leading full-line `--` comments and blank lines are
/// ignored when matching, so a directive may be preceded by comment lines in
/// the same chunk.
func chunkSetTerm(_ chunk: String) -> String? {
    var meaningful: [String] = []
    for line in chunk.split(separator: "\n", omittingEmptySubsequences: false) {
        let stripped = line.trimmingCharacters(in: .whitespaces)
        if stripped.isEmpty || stripped.hasPrefix("--") { continue }
        meaningful.append(stripped)
    }
    if meaningful.isEmpty { return nil }
    let joined = meaningful.joined(separator: " ")
    let lower = joined.lowercased()
    guard lower.hasPrefix("set term") else { return nil }
    // Capture the remainder after "set term" (preserve original casing) and trim.
    let rest = joined.dropFirst("set term".count).trimmingCharacters(in: .whitespaces)
    return rest.isEmpty ? nil : rest
}

/// Split SQL into top-level statements on the active terminator.
///
/// Quote-aware (single/double quotes) and `--` comment-aware. Honors the
/// `SET TERM <terminator>` client directive (Firebird / `sb_isql` semantics):
/// the directive changes the active terminator and is consumed — it is not
/// emitted as a statement and is not counted. With no `SET TERM` directive
/// present, the behavior is identical to a plain quote-aware top-level `;`
/// split, so existing scripts and statement indices are unchanged.
func splitStatements(_ script: String) -> [String] {
    var statements: [String] = []
    var term = ";"
    var buf = ""
    var inSingle = false
    var inDouble = false
    let chars = Array(script)
    let length = chars.count
    var i = 0

    func flush() {
        let chunk = buf.trimmingCharacters(in: .whitespacesAndNewlines)
        if chunk.isEmpty { return }
        if let newTerm = chunkSetTerm(chunk) {
            term = newTerm
            return
        }
        statements.append(chunk)
    }

    func matchesTerm(at pos: Int) -> Bool {
        let t = Array(term)
        if t.isEmpty || pos + t.count > length { return false }
        for k in 0..<t.count where chars[pos + k] != t[k] { return false }
        return true
    }

    while i < length {
        let ch = chars[i]
        if !inSingle && !inDouble && ch == "-" && i + 1 < length && chars[i + 1] == "-" {
            // `--` line comment: consume to end of line verbatim, without scanning
            // for the terminator or quotes inside it.
            var eol = i
            while eol < length && chars[eol] != "\n" { eol += 1 }
            buf.append(contentsOf: chars[i..<eol])
            i = eol
            continue
        }
        if ch == "'" && !inDouble {
            inSingle.toggle()
            buf.append(ch)
            i += 1
            continue
        }
        if ch == "\"" && !inSingle {
            inDouble.toggle()
            buf.append(ch)
            i += 1
            continue
        }
        if !inSingle && !inDouble && matchesTerm(at: i) {
            let matchedLen = term.count  // capture before flush(), which may change term
            flush()
            buf = ""
            i += matchedLen
            continue
        }
        buf.append(ch)
        i += 1
    }
    flush()
    return statements
}

func classify(_ sql: String) -> String {
    let trimmed = sql.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    let first = trimmed.split(separator: " ").first.map(String.init) ?? ""
    if ["create", "alter", "drop"].contains(first) { return "ddl" }
    if ["insert", "update", "delete", "merge", "upsert"].contains(first) { return "dml" }
    if ["commit", "rollback", "savepoint", "begin", "start"].contains(first) { return "transaction" }
    if ["grant", "revoke"].contains(first) { return "security_refusal" }
    return trimmed.contains("sys.") ? "metadata" : "query"
}

func readInput(_ path: String) throws -> String {
    if path == "-" { return String(data: FileHandle.standardInput.readDataToEndOfFile(), encoding: .utf8) ?? "" }
    return try String(contentsOfFile: path)
}

func nowNs() -> Int64 { Int64(Date().timeIntervalSince1970 * 1_000_000_000) }
func addTiming(_ timings: inout [String: Int64], _ group: String, _ started: Int64) { timings[group, default: 0] += nowNs() - started }
func sha256Text(_ text: String) -> String { "sha256:" + SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined() }
func writeText(_ path: String, _ text: String) throws { try FileManager.default.createDirectory(atPath: URL(fileURLWithPath: path).deletingLastPathComponent().path, withIntermediateDirectories: true); try text.write(toFile: path, atomically: false, encoding: .utf8) }
func appendText(_ path: String, _ text: String) throws { try FileManager.default.createDirectory(atPath: URL(fileURLWithPath: path).deletingLastPathComponent().path, withIntermediateDirectories: true); let data = Data(text.utf8); if FileManager.default.fileExists(atPath: path) { let h = try FileHandle(forWritingTo: URL(fileURLWithPath: path)); try h.seekToEnd(); try h.write(contentsOf: data); try h.close() } else { try data.write(to: URL(fileURLWithPath: path)) } }
func appendJsonl(_ path: String, _ value: [String: Any?]) throws { try appendText(path, jsonLine(value) + "\n") }
func jsonArray(_ rows: [[String: Any?]]) -> String { "[" + rows.map(jsonLine).joined(separator: ",") + "]" }
func jsonLine(_ value: [String: Any?]) -> String { "{" + value.map { "\"\($0.key)\":\(jsonValue($0.value))" }.joined(separator: ",") + "}" }
func jsonLine(_ value: [String: Int64]) -> String { "{" + value.map { "\"\($0.key)\":\($0.value)" }.joined(separator: ",") + "}" }
func jsonLine(_ value: [String: Int]) -> String { "{" + value.map { "\"\($0.key)\":\($0.value)" }.joined(separator: ",") + "}" }
func jsonValue(_ value: Any?) -> String {
    guard let value else { return "null" }
    if let text = value as? String { return "\"\(text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\"" }
    if let bool = value as? Bool { return bool ? "true" : "false" }
    if let int = value as? Int { return "\(int)" }
    if let int64 = value as? Int64 { return "\(int64)" }
    if let array = value as? [Any?] { return "[" + array.map(jsonValue).joined(separator: ",") + "]" }
    return "\"\(String(describing: value))\""
}
